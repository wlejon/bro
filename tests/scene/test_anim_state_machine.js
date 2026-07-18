// Skeletal animation state machine on SkinnedMeshNode — addStateMachine /
// travel / wildcard transitions / autoAdvance / syncPhase / suspension by
// manual play() / node.state / onStateChanged / blendState().state.
// Exercises the machine tier in src/scene/animation_player.cpp and the
// bindings in src/js/scene_bindings_anim.cpp.
//
// Rig: the same 2-bone hinge as test_skeletal_animation.js (bone 1 at
// (0,1,0)). Clips animate only bone 1's rotation about distinct axes, so
// fades are checkable against the same rigging pipeline the player uses
// (Animation.evaluate → Pose.blend → computeWorldMatrices). All time
// advance goes through advanceTime() virtual time.

function cmpMat(actual, expected, tol, label) {
    assert(actual !== null && actual !== undefined && actual.length === 16,
        `${label}: got a 16-float matrix`);
    for (let i = 0; i < 16; i++) {
        assert(Math.abs(actual[i] - expected[i]) < tol,
            `${label}: [${i}] ${actual[i]} vs ${expected[i]}`);
    }
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
    console.log('scene context not available (no GPU) — skipping state machine test');
} else {
    // ------------------------------------------------------------------
    // Minimal 2-bone rig + clips
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
    const idleClip  = mkClip('idleClip', 1.0, [0, 0, 0, 1]);       // identity
    const bendClip  = mkClip('bendClip', 1.0, [0, 0, s45, s45]);   // Z
    const twistClip = mkClip('twistClip', 1.0, [0, s45, 0, s45]);  // Y
    const jumpClip  = mkClip('jumpClip', 0.5, [s45, 0, 0, s45]);   // X, one-shot

    const node = scene.createSkinnedMesh({ positions, normals, indices,
                                           color: 'red', skin });
    node.setSkeleton(skel);
    node.addClip('idleClip', idleClip).addClip('bendClip', bendClip)
        .addClip('twistClip', twistClip).addClip('jumpClip', jumpClip);
    node.addBlendSpace1D('moveSpace', [
        { clip: 'bendClip', pos: 0 },
        { clip: 'twistClip', pos: 1 },
    ]);

    const evalClip = (clip, t, loop) => clip.evaluate(skel, t, { loop });
    const bone1 = (pose) => Array.from(pose.computeWorldMatrices(skel).slice(16, 32));

    // ------------------------------------------------------------------
    // No machine yet: state is null, travel throws
    // ------------------------------------------------------------------
    assert(node.state === null, 'state is null before addStateMachine');
    assert(node.blendState().state === null, 'blendState().state null too');
    let threw = false;
    try { node.travel('idle'); } catch (e) { threw = true; }
    assert(threw, 'travel before addStateMachine throws');

    // ------------------------------------------------------------------
    // Validation
    // ------------------------------------------------------------------
    threw = false;
    try {
        node.addStateMachine({ states: [{ name: 'a', source: 'nope' }] });
    } catch (e) { threw = true; }
    assert(threw, 'unknown state source throws');
    threw = false;
    try {
        node.addStateMachine({
            states: [{ name: 'a', source: 'idleClip' }],
            transitions: [{ from: 'a', to: 'ghost' }],
        });
    } catch (e) { threw = true; }
    assert(threw, 'transition to unknown state throws');
    threw = false;
    try {
        node.addStateMachine({ states: [{ name: 'a', source: 'idleClip' }],
                               initial: 'ghost' });
    } catch (e) { threw = true; }
    assert(threw, 'unknown initial state throws');
    threw = false;
    try {
        node.addStateMachine({ states: [{ name: 'a', source: 'idleClip' },
                                        { name: 'a', source: 'bendClip' }] });
    } catch (e) { threw = true; }
    assert(threw, 'duplicate state name throws');

    // ------------------------------------------------------------------
    // Install: enters the initial state immediately
    // ------------------------------------------------------------------
    const changes = [];
    node.onStateChanged = (from, to) => changes.push([from, to]);

    node.addStateMachine({
        states: [
            { name: 'idle',     source: 'idleClip' },
            { name: 'walk',     source: 'bendClip' },
            { name: 'jump',     source: 'jumpClip', loop: false },
            { name: 'move',     source: 'moveSpace' },
            { name: 'moveFast', source: 'moveSpace', speed: 2 },
        ],
        transitions: [
            { from: 'idle', to: 'walk', fade: 0.5 },
            { from: 'walk', to: 'idle', fade: 0.25 },
            { from: '*',    to: 'jump', fade: 0.3 },
            { from: 'jump', to: 'idle', fade: 0.2, autoAdvance: true },
            { from: 'idle', to: 'move', fade: 0.2 },
            { from: 'move', to: 'moveFast', fade: 0.2, syncPhase: true },
        ],
        initial: 'idle',
    });
    assert(node.state === 'idle', 'initial state entered');
    assert(node.blendState().state === 'idle', 'blendState carries the state');
    assert(node.currentAnimation === 'idleClip', 'initial source on the base track');
    assert(changes.length === 0, 'initial entry does not fire onStateChanged');

    // travel() to the current state is a no-op
    node.travel('idle');
    assert(node.state === 'idle' && changes.length === 0, 'travel(current) no-op');

    threw = false;
    try { node.travel('ghost'); } catch (e) { threw = true; }
    assert(threw, 'travel to unknown state throws');

    // ------------------------------------------------------------------
    // travel(): the defined fade drives a checkable crossfade
    // ------------------------------------------------------------------
    advanceTime(100);
    node.travel('walk');            // idle -> walk uses fade 0.5
    assert(node.state === 'walk', 'state switches at travel time');
    assert(changes.length === 1 && changes[0][0] === 'idle' && changes[0][1] === 'walk',
        `onStateChanged (idle -> walk), got ${JSON.stringify(changes)}`);

    advanceTime(250);               // fade alpha 0.5, walk clip at 0.25
    {
        const from = evalClip(idleClip, 0, true);   // identity throughout
        const to   = evalClip(bendClip, 0.25, true);
        Pose.blend(from, to, 0.5);
        cmpMat(node.getBoneWorldMatrix(1), bone1(from), 1e-3,
            'mid-fade pose matches Pose.blend ground truth');
        const w = Object.fromEntries(node.blendState().clips.map(c => [c.name, c.weight]));
        near(w.idleClip, 0.5, 5e-3, 'outgoing weight 0.5 mid-fade');
        near(w.bendClip, 0.5, 5e-3, 'incoming weight 0.5 mid-fade');
    }
    advanceTime(400);               // fade complete (0.65 > 0.5)
    {
        const gt = evalClip(bendClip, node.animationTime, true);
        cmpMat(node.getBoneWorldMatrix(1), bone1(gt), 1e-3,
            'pose converges to the target state after the fade');
        const st = node.blendState();
        assert(st.clips.length === 1 && st.clips[0].name === 'bendClip',
            'only the target clip remains after the fade');
    }

    // ------------------------------------------------------------------
    // Wildcard: walk has no direct transition to jump; '*' supplies one
    // (its 0.3 fade proves the wildcard was used, not the warn-direct path)
    // ------------------------------------------------------------------
    node.travel('jump');
    assert(node.state === 'jump', 'wildcard transition taken');
    advanceTime(150);               // mid-fade: both sources present
    {
        const names = node.blendState().clips.map(c => c.name).sort();
        assert(names.length === 2 && names[0] === 'bendClip' && names[1] === 'jumpClip',
            `wildcard fade in flight (${names})`);
    }

    // ------------------------------------------------------------------
    // autoAdvance: the non-looping jump clip ends -> machine returns to idle
    // ------------------------------------------------------------------
    const finishes = [];
    node.onAnimationFinished = (name) => finishes.push(name);
    changes.length = 0;
    advanceTime(450);               // jump (0.5 s) ends at 0.15+0.45
    assert(node.state === 'idle', 'autoAdvance fired at clip end');
    assert(changes.length === 1 && changes[0][0] === 'jump' && changes[0][1] === 'idle',
        `onStateChanged (jump -> idle), got ${JSON.stringify(changes)}`);
    assert(finishes.length === 1 && finishes[0] === 'jumpClip',
        'onAnimationFinished still fires alongside autoAdvance');
    advanceTime(500);
    assert(node.state === 'idle' && changes.length === 1,
        'autoAdvance fires exactly once');

    // ------------------------------------------------------------------
    // syncPhase: move -> moveFast carries the shared blend-space phase
    // ------------------------------------------------------------------
    node.travel('move');
    node.setBlendPos('moveSpace', 0.5);
    advanceTime(370);
    const phaseBefore = node.blendState().phase;
    assert(phaseBefore > 0.3, `phase advanced before the switch (${phaseBefore})`);
    node.travel('moveFast');        // syncPhase: true
    near(node.blendState().phase, phaseBefore, 2e-3,
        'syncPhase carries the phase into the incoming state');
    {
        const st = node.blendState();
        assert(st.pos[0] === 0.5, 'blend-space parameter survives the switch');
    }

    // Control: a transition without syncPhase restarts at phase 0.
    // moveFast -> move is undefined (warn + direct switch, fade 0).
    advanceTime(200);
    assert(node.blendState().phase > 0.05, 'phase advanced in moveFast');
    node.travel('move');
    near(node.blendState().phase, 0, 1e-6,
        'no syncPhase: incoming state restarts at phase 0');
    assert(node.state === 'move', 'warn-direct switch still lands in the state');
    assert(node.blendState().clips.length <= 2 &&
           node.blendState().clips.every(c => c.weight <= 1),
        'direct switch has no fade source');

    // ------------------------------------------------------------------
    // Manual play() suspends the machine; travel() re-enters it
    // ------------------------------------------------------------------
    node.play('twistClip');
    assert(node.state === null, 'manual play suspends the machine');
    assert(node.blendState().state === null, 'blendState reflects suspension');
    assert(node.currentAnimation === 'twistClip', 'manual clip took the base');

    changes.length = 0;
    node.travel('jump');            // '*' -> jump still applies from suspension
    assert(node.state === 'jump', 'travel re-enters the machine');
    assert(changes.length === 1 && changes[0][0] === null && changes[0][1] === 'jump',
        `re-entry fires (null -> jump), got ${JSON.stringify(changes)}`);

    // stop() also suspends
    node.stop();
    assert(node.state === null, 'stop() suspends the machine');
    node.travel('idle');            // no transition from suspension -> warn+direct
    assert(node.state === 'idle', 'travel from stopped re-enters directly');
    assert(node.isPlaying === true, 'machine re-entry restarts playback');

    console.log('animation state machine tests passed');
}
