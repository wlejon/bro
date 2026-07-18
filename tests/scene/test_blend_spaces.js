// Skeletal blend spaces (1D/2D) + layered blending on SkinnedMeshNode —
// addBlendSpace1D/2D / setBlendPos / phase sync / playLayer stack /
// setLayerWeight / stopLayer / blendState / crossfade into a blend space.
// Exercises src/scene/animation_player.cpp, bromesh::blendPosesN, and the
// bindings in src/js/scene_bindings_anim.cpp.
//
// Rig: the same 2-bone hinge as test_skeletal_animation.js (bone 1 at
// (0,1,0)). All clips animate only bone 1's rotation, each about a distinct
// axis and with a distinct duration, so every blend result is checkable
// against the SAME rigging pipeline the player uses internally:
// Animation.evaluate → Pose.blendN → Pose.blend → computeWorldMatrices.
// All time advance goes through advanceTime() virtual time.

function cmpMat(actual, expected, tol, label) {
    assert(actual !== null && actual !== undefined && actual.length === 16,
        `${label}: got a 16-float matrix`);
    for (let i = 0; i < 16; i++) {
        assert(Math.abs(actual[i] - expected[i]) < tol,
            `${label}: [${i}] ${actual[i]} vs ${expected[i]}`);
    }
}

function matFinite(m, label) {
    assert(m !== null && m.length === 16, `${label}: got a matrix`);
    for (let i = 0; i < 16; i++)
        assert(Number.isFinite(m[i]), `${label}: [${i}] finite (${m[i]})`);
}

function near(a, b, tol, label) {
    assert(Math.abs(a - b) < tol, `${label}: ${a} vs ${b}`);
}

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping blend space test');
} else {
    // ------------------------------------------------------------------
    // Minimal 2-bone rig
    // ------------------------------------------------------------------
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
    const mkClip = (name, dur, quat) => new Animation({
        name, duration: dur,
        channels: [{ boneIndex: 1, path: 'rotation',
                     times: new Float32Array([0, dur]),
                     values: new Float32Array([0, 0, 0, 1, ...quat]) }],
    });
    const walkClip = mkClip('walk', 1.0, [0, 0, s45, s45]);   // Z, 1.0 s cycle
    const runClip  = mkClip('run',  0.5, [0, s45, 0, s45]);   // Y, 0.5 s cycle
    const jogClip  = mkClip('jog',  0.8, [s45, 0, 0, s45]);   // X, 0.8 s cycle
    const waveClip = mkClip('wave', 1.0, [s45, 0, 0, s45]);   // X, 1.0 s

    const node = scene.createSkinnedMesh({ positions, normals, indices,
                                           color: 'red', skin });
    node.setSkeleton(skel);
    node.addClip('walk', walkClip).addClip('run', runClip)
        .addClip('jog', jogClip).addClip('wave', waveClip);

    const evalClip = (clip, t) => clip.evaluate(skel, t, { loop: true });
    const bone1 = (pose) => Array.from(pose.computeWorldMatrices(skel).slice(16, 32));

    // ------------------------------------------------------------------
    // Pose.blendN sanity: N=2 exactly matches Pose.blend
    // ------------------------------------------------------------------
    {
        const a = evalClip(walkClip, 0.3), b = evalClip(runClip, 0.2);
        const n = Pose.blendN([a, b], [0.25, 0.75]);
        const g = a.clone(); Pose.blend(g, b, 0.75);
        for (let i = 0; i < g.data.length; i++)
            assert(n.data[i] === g.data[i], 'Pose.blendN N=2 === Pose.blend');
    }

    // ------------------------------------------------------------------
    // 1D blend space: registration + validation
    // ------------------------------------------------------------------
    let threw = false;
    try { node.addBlendSpace1D('bad', [{ clip: 'nope', pos: 0 }]); }
    catch (e) { threw = true; }
    assert(threw, 'addBlendSpace1D with unknown clip throws');
    threw = false;
    try { node.addBlendSpace1D('bad', []); } catch (e) { threw = true; }
    assert(threw, 'addBlendSpace1D with no points throws');

    assert(node.addBlendSpace1D('locomotion', [
        { clip: 'run', pos: 2 },        // deliberately unsorted — sorted inside
        { clip: 'walk', pos: 0 },
    ]) === node, 'addBlendSpace1D chains');

    // ------------------------------------------------------------------
    // play() a blend space; endpoint pose is exactly the endpoint clip
    // ------------------------------------------------------------------
    node.play('locomotion');
    assert(node.currentAnimation === 'locomotion', 'space is the base track');
    assert(node.isPlaying === true, 'space is playing');
    {
        const st = node.blendState();
        assert(st.pos.length === 1 && st.pos[0] === 0,
            '1D space starts at its min position');
        near(st.phase, 0, 1e-6, 'phase starts at 0');
    }
    advanceTime(250);   // at pos 0 the cycle duration is walk's 1.0 s
    {
        const st = node.blendState();
        near(st.phase, 0.25, 2e-3, 'phase advances on the walk cycle');
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        near(w.walk, 1, 1e-6, 'endpoint: walk weight 1');
        near(w.run ?? 0, 0, 1e-6, 'endpoint: run weight 0');
        cmpMat(node.getBoneWorldMatrix(1), bone1(evalClip(walkClip, st.phase * 1.0)),
            1e-5, 'endpoint pose == walk clip exactly');
        near(node.animationDuration, 1.0, 1e-6, 'duration == walk cycle at pos 0');
    }

    // ------------------------------------------------------------------
    // Interior positions: two-neighbor blend matches Pose.blendN, and the
    // shared phase samples each clip at ITS OWN duration (phase sync)
    // ------------------------------------------------------------------
    for (const [x, wWalk] of [[0.5, 0.75], [1.0, 0.5], [1.5, 0.25]]) {
        node.setBlendPos('locomotion', x);
        const st = node.blendState();
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        near(w.walk, wWalk, 1e-5, `pos ${x}: walk weight`);
        near(w.run, 1 - wWalk, 1e-5, `pos ${x}: run weight`);
        near(w.walk + w.run, 1, 1e-6, `pos ${x}: weights sum to 1`);
        const gt = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5)],
            [wWalk, 1 - wWalk]);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-4,
            `pos ${x}: blended pose matches Pose.blendN at the shared phase`);
    }

    // ------------------------------------------------------------------
    // Phase-locked advance mid-blend: cycle duration is the weighted mix
    // (0.5*1.0 + 0.5*0.5 = 0.75 s), both clips stay foot-aligned
    // ------------------------------------------------------------------
    node.play('locomotion');            // restart at phase 0 (pos persists)
    node.setBlendPos('locomotion', 1);
    near(node.animationDuration, 0.75, 1e-5, 'blended cycle duration at 50/50');
    advanceTime(300);                   // phase = 0.3 / 0.75 = 0.4
    {
        const st = node.blendState();
        near(st.phase, 0.4, 2e-3, 'phase integrates on the blended duration');
        const gt = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5)],
            [0.5, 0.5]);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-4,
            'mid-blend pose: both clips sampled at one shared phase');
    }
    advanceTime(600);                   // 0.9 / 0.75 = 1.2 → wraps to 0.2
    near(node.blendState().phase, 0.2, 3e-3, 'phase wraps at 1');

    // ------------------------------------------------------------------
    // Clamping + timescale
    // ------------------------------------------------------------------
    node.setBlendPos('locomotion', 99);
    assert(node.blendState().pos[0] === 2, '1D position clamps to max');
    cmpMat(node.getBoneWorldMatrix(1),
        bone1(evalClip(runClip, node.blendState().phase * 0.5)), 1e-5,
        'clamped position == pure run');
    node.setBlendPos('locomotion', -7);
    assert(node.blendState().pos[0] === 0, '1D position clamps to min');

    node.addBlendSpace1D('loco2', [
        { clip: 'walk', pos: 0 },
        { clip: 'run',  pos: 2, timescale: 2 },   // run counts as a 0.25 s cycle
    ]);
    node.play('loco2');
    node.setBlendPos('loco2', 2);
    near(node.animationDuration, 0.25, 1e-5, 'timescale shortens the cycle');
    node.setBlendPos('loco2', 1);
    near(node.animationDuration, 0.5 * 1.0 + 0.5 * 0.25, 1e-5,
        'timescale mixes into the blended duration');

    // ------------------------------------------------------------------
    // Scrub a blend space through animationTime (phase = t / duration)
    // ------------------------------------------------------------------
    node.play('locomotion');
    node.setBlendPos('locomotion', 1);
    node.animationTime = 0.375;         // duration 0.75 → phase 0.5
    near(node.blendState().phase, 0.5, 1e-5, 'scrub sets the phase');

    // ------------------------------------------------------------------
    // Crossfade INTO a blend space from a clip (existing fade machinery)
    // ------------------------------------------------------------------
    node.play('walk', { loop: false });
    advanceTime(250);                          // walk at 0.25
    node.setBlendPos('locomotion', 1);
    node.play('locomotion', { fadeTime: 0.5 });
    assert(node.currentAnimation === 'locomotion', 'fade target is the space');
    advanceTime(250);                          // alpha 0.5; space phase 0.25/0.75
    {
        const st = node.blendState();
        // walk appears twice: outgoing clip (0.5) + space member (0.25).
        let sum = 0;
        for (const c of st.clips) sum += c.weight;
        near(sum, 1, 1e-5, 'crossfade weights sum to 1');
        const wRun = st.clips.find(c => c.name === 'run').weight;
        near(wRun, 0.25, 1e-3, 'incoming space run share');
        const from = evalClip(walkClip, 0.5);  // kept advancing while fading
        const space = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5)],
            [0.5, 0.5]);
        Pose.blend(from, space, 0.5);
        cmpMat(node.getBoneWorldMatrix(1), bone1(from), 1e-3,
            'mid-fade pose blends outgoing clip with the space');
    }
    advanceTime(400);                          // fade completes
    {
        const st = node.blendState();
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        assert(!('walk' in w) || Math.abs(w.walk - 0.5) < 1e-3,
            'after the fade only space members remain weighted');
        const gt = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5)],
            [0.5, 0.5]);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-3,
            'crossfade converges to the pure blend space');
    }

    // ------------------------------------------------------------------
    // 2D blend space: nearest-3 inverse-square-distance weights
    // ------------------------------------------------------------------
    node.addBlendSpace2D('strafe', [
        { clip: 'walk', pos: [0, 0] },
        { clip: 'run',  pos: [2, 0] },
        { clip: 'jog',  pos: [0, 2] },
    ]);
    node.play('strafe');

    // Exactly on a sample point: that clip alone.
    node.setBlendPos('strafe', 2, 0);
    {
        const st = node.blendState();
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        near(w.run, 1, 1e-6, 'sample point: run weight 1');
        assert(st.pos.length === 2 && st.pos[0] === 2 && st.pos[1] === 0,
            '2D pos reads back');
        cmpMat(node.getBoneWorldMatrix(1),
            bone1(evalClip(runClip, st.phase * 0.5)), 1e-4,
            'sample point pose == that clip');
    }

    // Equidistant center: 1/3 each; player result == Pose.blendN 3-way nlerp.
    node.setBlendPos('strafe', [1, 1]);        // array form
    {
        const st = node.blendState();
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        let sum = 0;
        for (const c of st.clips) sum += c.weight;
        near(sum, 1, 1e-5, '2D weights sum to 1');
        near(w.walk, 1 / 3, 1e-5, 'equidistant: walk 1/3');
        near(w.run,  1 / 3, 1e-5, 'equidistant: run 1/3');
        near(w.jog,  1 / 3, 1e-5, 'equidistant: jog 1/3');
        const gt = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5),
             evalClip(jogClip, st.phase * 0.8)],
            [w.walk, w.run, w.jog]);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-4,
            '3-way pose matches Pose.blendN (weighted nlerp)');
    }

    // Near a point: inverse-square-distance favors it strongly.
    node.setBlendPos('strafe', 0.2, 0.2);
    {
        const st = node.blendState();
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        // d² = 0.08 / 3.28 / 3.28 → IDW(p=2) weights:
        const iw = [1 / 0.08, 1 / 3.28, 1 / 3.28];
        const s = iw[0] + iw[1] + iw[2];
        near(w.walk, iw[0] / s, 1e-4, 'IDW: walk weight formula');
        near(w.run,  iw[1] / s, 1e-4, 'IDW: run weight formula');
        assert(w.walk > 0.9, 'nearest point dominates');
    }

    // Phase sync in 2D: blended duration = Σ wᵢ·durᵢ.
    node.play('strafe');
    node.setBlendPos('strafe', 1, 1);
    near(node.animationDuration, (1.0 + 0.5 + 0.8) / 3, 1e-4,
        '2D blended cycle duration');
    advanceTime(230);
    {
        const st = node.blendState();
        near(st.phase, 0.23 / ((1.0 + 0.5 + 0.8) / 3), 3e-3, '2D phase integrates');
        const gt = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5),
             evalClip(jogClip, st.phase * 0.8)],
            [1 / 3, 1 / 3, 1 / 3]);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-4,
            '2D advance stays phase-locked across three durations');
    }

    // ------------------------------------------------------------------
    // Degenerate 2D layouts must not NaN
    // ------------------------------------------------------------------
    node.addBlendSpace2D('coincident', [
        { clip: 'walk', pos: [0, 0] },
        { clip: 'run',  pos: [0, 0] },     // duplicate position
        { clip: 'jog',  pos: [1, 0] },
    ]);
    node.play('coincident');
    node.setBlendPos('coincident', 0, 0);  // ON the duplicated point
    {
        const st = node.blendState();
        const w = Object.fromEntries(st.clips.map(c => [c.name, c.weight]));
        near(w.walk, 0.5, 1e-6, 'coincident points split the weight');
        near(w.run, 0.5, 1e-6, 'coincident points split the weight (2)');
        matFinite(node.getBoneWorldMatrix(1), 'coincident-point pose');
    }
    advanceTime(100);
    matFinite(node.getBoneWorldMatrix(1), 'coincident-point pose after advance');

    node.addBlendSpace2D('collinear', [
        { clip: 'walk', pos: [0, 0] },
        { clip: 'run',  pos: [1, 0] },
        { clip: 'jog',  pos: [2, 0] },
    ]);
    node.play('collinear');
    for (const p of [[0.5, 5], [1, 0], [-3, -3], [1.5, 0]]) {
        node.setBlendPos('collinear', p[0], p[1]);
        matFinite(node.getBoneWorldMatrix(1), `collinear pose at ${p}`);
        let sum = 0;
        for (const c of node.blendState().clips) sum += c.weight;
        near(sum, 1, 1e-5, `collinear weights sum to 1 at ${p}`);
    }

    // ------------------------------------------------------------------
    // Layer stack: ordered masked layers over the base
    // ------------------------------------------------------------------
    const mask1 = new Uint8Array([0, 1]);
    node.play('walk', { loop: false });
    advanceTime(1050);                        // base holds walk end (Rz90)

    node.playLayer(1, 'wave', { mask: mask1, weight: 0.6 });
    node.playLayer(2, 'run',  { mask: mask1, weight: 0.5 });
    advanceTime(200);                         // wave at 0.2, run at 0.2
    {
        const st = node.blendState();
        assert(st.layers.length === 2, 'two active layers');
        assert(st.layers[0].slot === 1 && st.layers[0].name === 'wave',
            'layer slot 1 reported');
        assert(st.layers[1].slot === 2 && st.layers[1].name === 'run',
            'layer slot 2 reported');
        near(st.layers[0].weight, 0.6, 1e-6, 'layer weight reported');

        const gt = walkClip.evaluate(skel, 1, { loop: false });
        Pose.blend(gt, evalClip(waveClip, 0.2), 0.6, mask1);
        Pose.blend(gt, evalClip(runClip, 0.2), 0.5, mask1);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-4,
            'layers compose in ascending slot order with masks');
        cmpMat(node.getBoneWorldMatrix(0),
            [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], 1e-5,
            'masked-out root bone untouched by both layers');
    }

    // Runtime weight change takes effect immediately.
    node.setLayerWeight(1, 0.25);
    {
        const gt = walkClip.evaluate(skel, 1, { loop: false });
        Pose.blend(gt, evalClip(waveClip, 0.2), 0.25, mask1);
        Pose.blend(gt, evalClip(runClip, 0.2), 0.5, mask1);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-4,
            'setLayerWeight re-poses instantly');
    }

    // stopLayer with fade: weight ramps to 0, then the slot frees.
    node.stopLayer(1, { fadeTime: 0.4 });
    advanceTime(200);                         // half faded: 0.25 → 0.125
    {
        const st = node.blendState();
        const l1 = st.layers.find(l => l.slot === 1);
        near(l1.weight, 0.125, 2e-3, 'stopLayer fade halves the weight');
    }
    advanceTime(300);                         // fade done
    {
        const st = node.blendState();
        assert(st.layers.length === 1 && st.layers[0].slot === 2,
            'faded-out layer slot freed');
    }

    // Legacy masked play() is layer slot 0.
    node.play('wave', { mask: mask1 });
    {
        const st = node.blendState();
        assert(st.layers.some(l => l.slot === 0 && l.name === 'wave'),
            'play(name, {mask}) lands on layer slot 0');
    }
    assert(node.currentAnimation === 'walk',
        'masked play does not replace the base track');

    // Layer fade-in.
    node.playLayer(3, 'jog', { mask: mask1, weight: 1, fadeTime: 0.5 });
    advanceTime(250);
    {
        const st = node.blendState();
        const l3 = st.layers.find(l => l.slot === 3);
        near(l3.weight, 0.5, 2e-3, 'layer fade-in at 50%');
    }

    // One-shot layer expires and fires onAnimationFinished with its name.
    let finishes = [];
    node.onAnimationFinished = (name) => finishes.push(name);
    node.playLayer(4, 'run', { mask: mask1, loop: false });
    advanceTime(600);                         // past run's 0.5 s
    assert(finishes.includes('run'), 'one-shot layer fires finished');
    assert(!node.blendState().layers.some(l => l.slot === 4),
        'one-shot layer slot freed on finish');

    // Bad slots / names throw; the layer cap is enforced.
    threw = false;
    try { node.playLayer(8, 'wave', {}); } catch (e) { threw = true; }
    assert(threw, 'playLayer beyond kMaxLayers throws');
    threw = false;
    try { node.playLayer(-1, 'wave', {}); } catch (e) { threw = true; }
    assert(threw, 'negative slot throws');
    threw = false;
    try { node.playLayer(0, 'locomotion', {}); } catch (e) { threw = true; }
    assert(threw, 'blend spaces are base-track only (playLayer throws)');
    threw = false;
    try { node.setLayerWeight(6, 1); } catch (e) { threw = true; }
    assert(threw, 'setLayerWeight on an empty slot throws');

    // Layers compose over a blend-space base unchanged (fresh stack).
    node.stop();
    node.play('locomotion', { fadeTime: 0 });
    node.setBlendPos('locomotion', 1);
    node.playLayer(1, 'wave', { mask: mask1, weight: 0.7 });
    advanceTime(150);
    {
        const st = node.blendState();
        const base = Pose.blendN(
            [evalClip(walkClip, st.phase * 1.0), evalClip(runClip, st.phase * 0.5)],
            [0.5, 0.5]);
        const l1 = st.layers.find(l => l.slot === 1);
        Pose.blend(base, evalClip(waveClip, l1.phase * 1.0), 0.7, mask1);
        cmpMat(node.getBoneWorldMatrix(1), bone1(base), 1e-4,
            'masked layer composes over a blend-space base');
    }

    // stop() clears the whole stack back to bind pose.
    node.stop();
    assert(node.blendState().clips.length === 0, 'stop clears blendState');
    cmpMat(node.getBoneWorldMatrix(1),
        [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,1,0,1], 1e-5, 'stop returns to bind pose');

    console.log('blend space + layered blending tests passed');
}
